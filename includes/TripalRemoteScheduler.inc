<?php
/**
 * The declaration of TripalRemoteScheduler Class.
 *
 */
class TripalRemoteScheduler {

  // The list of jobs currently in the queue either waiting or running.
  private $queue = array();

  // The list of resources available for running jobs.
  private $resources = array();


  /**
   * Implements the class constructor.
   */
  public function __construct() {
    // Get the list of available resources, and rank them in order of
    // prefered use. Local is always available.
    module_load_include('inc', 'tripal_remote_job', 'includes/TripalRemoteResource');
    $this->resources = TripalRemoteResource::getResources();

    // Popuplate the job queue.
    $this->refreshQueue();
  }

  /**
   * Implements the class destructor.
   */
  public function __destruct() {
  }


  /**
   * Refreshes the job queue of the scheduler.
   */
  protected function refreshQueue() {

    // First, Clean up any completed, cancelled or errored jobs:
    $sql = "
      DELETE FROM {tripal_remote_job} WHERE job_id IN (
        SELECT TRJ.job_id
        FROM {tripal_remote_job} TRJ
          INNER JOIN {tripal_jobs} TJ on TJ.job_id = TRJ.job_id
        WHERE NOT TJ.status = 'Waiting' AND NOT TJ.status = 'Running'
        ORDER BY TJ.submit_date ASC)
    ";
    $results = db_query($sql);

    // Second, get the jobs that are left.
    $sql = "
      SELECT TRJ.job_id, TRJ.resource_id
      FROM {tripal_remote_job} TRJ
        INNER JOIN {tripal_jobs} TJ on TJ.job_id = TRJ.job_id
      ORDER BY TJ.submit_date ASC
    ";
    $results = db_query($sql);
    while ($record = $results->fetchObject()) {
      $job = new TripalRemoteJob();
      $job->load($record->job_id);
      $this->queue[] = $job;
    }
  }

  /**
   * Execute a remote job.
   */
  public function launchJob(TripalRemoteJob $job) {
    $hold = array();
    $num_user_jobs = 0;
    $max_jobs = 7;
    $done = FALSE;
    $wait_sleep = 60;
    $this_uid = $job->getUID();
    $user = user_load($this_uid);

    // Update the job queue.
    $this->refreshQueue();


    // Loop continously until we can schedule the job on a resource.
    while (!$done) {

      // Determine how manu jobs the user is currently running
      foreach ($this->queue as $qjob) {
        if ($qjob->getUID() == $this_uid) {
          $num_user_jobs++;
        }
      }
      if ($num_user_jobs > $max_jobs) {
        $job->logMessage($user->name . " is running more than $max_jobs jobs. Waiting for jobs to finish");
        sleep($wait_sleep);
        continue;
      }

      // Find a remote resource for this job.
      $this->assignJobResource($job);
      $resource = $job->getResource();
      if (!$resource) {
        throw new Exception("Could not assign a remote execution resource to the job.");
      }

      // Start the job!
      if ($job->getSendEmail() == TRUE) {
        $this->sendStartEmail($job);
      }
      $job->setProgress(0);
      $resource->run($job);

      $done = TRUE;
    }
  }

  /**
   * Checks the status of the running job.
   *
   * @param TripalRemoteJob $job
   *
   * @return
   *   A status string indicating: 'running', or 'completed'.
   *   If an error occurs then an exception is thrown.
   *
   * @throws Exception
   */
  public function checkJobStatus(TripalRemoteJob $job) {
    $resource = $job->getResource();
    if ($resource->isJobRunning($job)) {
      $job->logMessage('Job is still running....\n');
      return 'running';
    }
    return 'completed';
    $job->setProgress(100);
  }

  /**
   * Get Job results.
   * @param TripalRemoteJob $job
   */
  public function getResults(TripalRemoteJob $job) {
    $resource = $job->getResource();
    $resource->getJobResults($job);
  }

  /**
   * Cleans up the job.
   *
   * @param TripalRemoteJob $job
   */
  public function cleanJob(TripalRemoteJob $job) {
    $resource = $job->getResource();
    $resource->cleanJob($job);
    if ($job->getSendEmail() == TRUE) {
      $this->sendCompleteEmail($job);
    }
  }

  /**
   * Send the notification email (start).
   *
   * @param TripalRemoteJob $job
   */
  protected function sendStartEmail(TripalRemoteJob $job) {

    // Get the user for this job.
    $uid = $job->getUID();
    $user = user_load($uid);

    // All system mails need to specify the module and template key (mirrored from
    // hook_mail()) that the message they want to send comes from.
    $module = 'tripal_remote_job';
    $key = 'job-started';

    // Specify 'to' and 'from' addresses.
    $to = $user->getEmail();
    $from = variable_get('site_mail', 'admin@localhost.localdomain');

    // "params" loads in additional context for email content completion in
    // hook_mail(). In this case, we want to pass in the values the user entered
    // into the form, which include the message body in $form_values['message'].
    $params = array(
      'job'     => $job,
      'user'    => $user
    );

    // The language of the e-mail. This will one of three values:
    // * user_preferred_language(): Used for sending mail to a particular website
    //   user, so that the mail appears in their preferred language.
    // * global $language: Used when sending a mail back to the user currently
    //   viewing the site. This will send it in the language they're currently
    //   using.
    // * language_default(): Used when sending mail to a pre-existing, 'neutral'
    //   address, such as the system e-mail address, or when you're unsure of the
    //   language preferences of the intended recipient.
    //
    // Since in our case, we are sending a message to a random e-mail address that
    // is not necessarily tied to a user account, we will use the site's default
    // language.
    $language = language_default();

    // Whether or not to automatically send the mail when drupal_mail() is
    // called. This defaults to TRUE, and is normally what you want unless you
    // need to do additional processing before drupal_mail_send() is called.
    $send = TRUE;

    // Send the mail, and check for success. Note that this does not guarantee
    // message delivery; only that there were no PHP-related issues encountered
    // while sending.
    $result = drupal_mail($module, $key, $to, $language, $params, $from, $send);
    if ($result['result'] != TRUE) {
      watchdog('tripal_remote_job', t('There was a problem sending your message and it was not sent.'), array(), WATCHDOG_ERROR);
    }
  }

  /**
   * Send the notification email (completion).
   *
   * @param TripalRemoteJob $job
   */
  protected function sendCompleteEmail(TripalRemoteJob $job) {

    // Get the user for this job.
    $uid = $job->getUID();
    $user = user_load($uid);

    // All system mails need to specify the module and template key (mirrored from
    // hook_mail()) that the message they want to send comes from.
    $module = 'tripal_remote_job';
    $key = 'job-complete';

    // Specify 'to' and 'from' addresses.
    $to = $user->getEmail();
    $from = variable_get('site_mail', 'admin@localhost.localdomain');

    // "params" loads in additional context for email content completion in
    // hook_mail(). In this case, we want to pass in the values the user entered
    // into the form, which include the message body in $form_values['message'].
    $params = array(
      'job'     => $job,
      'user'    => $user,
    );

    // The language of the e-mail. This will one of three values:
    // * user_preferred_language(): Used for sending mail to a particular website
    //   user, so that the mail appears in their preferred language.
    // * global $language: Used when sending a mail back to the user currently
    //   viewing the site. This will send it in the language they're currently
    //   using.
    // * language_default(): Used when sending mail to a pre-existing, 'neutral'
    //   address, such as the system e-mail address, or when you're unsure of the
    //   language preferences of the intended recipient.
    //
    // Since in our case, we are sending a message to a random e-mail address that
    // is not necessarily tied to a user account, we will use the site's default
    // language.
    $language = language_default();

    // Whether or not to automatically send the mail when drupal_mail() is
    // called. This defaults to TRUE, and is normally what you want unless you
    // need to do additional processing before drupal_mail_send() is called.
    $send = TRUE;

    // Send the mail, and check for success. Note that this does not guarantee
    // message delivery; only that there were no PHP-related issues encountered
    // while sending.
    $result = drupal_mail($module, $key, $to, $language, $params, $from, $send);
    if ($result['result'] != TRUE) {
      watchdog('tripal_remote_job', t('There was a problem sending your message and it was not sent.'), array(), WATCHDOG_ERROR);
    }
  }

  /**
   * Assigns a resource to a job.
   *
   * @param TripalJob $job
   *
   * @return TripalRemoteResource
   */
  protected function assignJobResource(TripalRemoteJob $job) {
    foreach ($this->resources as $resource) {
      if ($resource->isEnabled() and $resource->getFreeSlots() > 0) {
        db_insert('tripal_remote_job')
          ->fields(array(
            'job_id' => $job->getJobID(),
            'resource_id' => $resource->getResourceID(),
          ))
          ->execute();
        $job->setResource($resource);
        return TRUE;
      }
    }
    return FALSE;
  }
}